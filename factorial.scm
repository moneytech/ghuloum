(labels
    ((fact
        (code (n)
            (if (zero? n)
                1
                (* n (labelcall fact (sub1 n)))))))
    (labelcall fact 5)
)
